#include "ghostdrive/app/text.hpp"

#include <optional>

namespace ghostdrive::app {

namespace {

// Decodes one UTF-8 sequence at `pos`, advancing it. Returns nullopt (and advances one byte)
// for an invalid, overlong or surrogate sequence.
std::optional<char32_t> decode(std::string_view s, std::size_t& pos) {
    auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    const unsigned char b0 = byte(pos);
    std::size_t len = 0;
    char32_t c = 0;
    char32_t min = 0;
    if (b0 < 0x80) {
        ++pos;
        return b0;
    } else if ((b0 & 0xE0) == 0xC0) {
        len = 2, c = b0 & 0x1Fu, min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3, c = b0 & 0x0Fu, min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4, c = b0 & 0x07u, min = 0x10000;
    } else {
        ++pos;
        return std::nullopt;
    }
    if (pos + len > s.size()) {
        ++pos;
        return std::nullopt;
    }
    for (std::size_t i = 1; i < len; ++i) {
        const unsigned char b = byte(pos + i);
        if ((b & 0xC0) != 0x80) {
            ++pos;
            return std::nullopt;
        }
        c = (c << 6) | (b & 0x3Fu);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        ++pos;
        return std::nullopt;
    }
    pos += len;
    return c;
}

void encode(char32_t c, std::string& out) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

bool in(char32_t c, char32_t lo, char32_t hi) {
    return c >= lo && c <= hi;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

}  // namespace

bool is_unsafe_code_point(char32_t c) {
    return c < 0x20 || in(c, 0x7F, 0x9F) || c == 0x00AD || in(c, 0x0600, 0x0605) || c == 0x061C ||
           c == 0x06DD || c == 0x070F || in(c, 0x0890, 0x0891) || c == 0x08E2 || c == 0x180E ||
           in(c, 0x200B, 0x200F) || in(c, 0x2028, 0x202E) || in(c, 0x2060, 0x2064) ||
           in(c, 0x2066, 0x206F) || in(c, 0xD800, 0xDFFF) || in(c, 0xE000, 0xF8FF) || c == 0xFEFF ||
           in(c, 0xFFF9, 0xFFFB) || c == 0x110BD || c == 0x110CD || in(c, 0x13430, 0x1343F) ||
           in(c, 0x1BCA0, 0x1BCA3) || in(c, 0x1D173, 0x1D17A) || in(c, 0xE0000, 0xE007F) ||
           in(c, 0xF0000, 0x10FFFF);
}

std::string clean_text(std::string_view text, std::size_t max_chars) {
    std::vector<char32_t> points;
    for (std::size_t pos = 0; pos < text.size();) {
        auto c = decode(text, pos);
        points.push_back(c && !is_unsafe_code_point(*c) ? *c : U'?');
    }
    bool cut = points.size() > max_chars;
    if (cut) points.resize(max_chars > 3 ? max_chars - 3 : 0);
    std::string out;
    for (char32_t c : points) encode(c, out);
    if (cut) out += "...";
    return out;
}

std::string trim(std::string_view text) {
    constexpr std::string_view space = " \t\r\n\f\v";
    auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) return {};
    auto last = text.find_last_not_of(space);
    return std::string(text.substr(first, last - first + 1));
}

std::vector<std::string> clean_lines(std::string_view text, std::size_t max_lines,
                                     std::size_t max_chars) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size() && out.size() < max_lines) {
        auto end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) end = text.size();
        std::string line = trim(text.substr(start, end - start));
        for (std::string_view prefix : {"ghostdrive: ", "error: "}) {
            if (starts_with(line, prefix))
                line = trim(std::string_view(line).substr(prefix.size()));
        }
        if (!line.empty()) out.push_back(clean_text(line, max_chars));
        start = end + 1;
    }
    return out;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        if (!out.empty()) out += '\n';
        out += line;
    }
    return out;
}

}  // namespace ghostdrive::app
