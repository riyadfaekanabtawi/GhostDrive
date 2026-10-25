#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ghostdrive::app {

// A parsed JSON value. Only what `ghostdrive status --json` needs: numbers are kept as integers
// when they have no fraction or exponent, and other numbers are accepted but read as 0.
struct JsonValue {
    enum class Kind { null, boolean, number, string, array, object };

    Kind kind = Kind::null;
    bool boolean = false;
    std::int64_t integer = 0;
    std::string string;
    std::vector<JsonValue> items;   // array elements, or object values
    std::vector<std::string> keys;  // object keys, parallel to items

    // The value of `key` in an object, or nullptr.
    const JsonValue* find(std::string_view key) const;

    bool is(Kind k) const { return kind == k; }
};

// Parses one JSON document (surrounding whitespace allowed). Returns nullopt on any syntax
// error, on nesting deeper than 32 levels, or on input over 1 MiB.
std::optional<JsonValue> parse_json(std::string_view text);

}  // namespace ghostdrive::app
