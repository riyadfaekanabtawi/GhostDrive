#include "ghostdrive/app/json.hpp"

#include <charconv>

namespace ghostdrive::app {

namespace {

constexpr std::size_t kMaxInput = 1 << 20;
constexpr int kMaxDepth = 32;

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    std::optional<JsonValue> document() {
        JsonValue v;
        if (!value(v, 0)) return std::nullopt;
        skip_space();
        if (pos_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    void skip_space() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip_space();
        if (pos_ >= s_.size()) return false;
        switch (s_[pos_]) {
            case 'n':
                out.kind = JsonValue::Kind::null;
                return literal("null");
            case 't':
                out.kind = JsonValue::Kind::boolean;
                out.boolean = true;
                return literal("true");
            case 'f':
                out.kind = JsonValue::Kind::boolean;
                out.boolean = false;
                return literal("false");
            case '"':
                out.kind = JsonValue::Kind::string;
                return string(out.string);
            case '[':
                out.kind = JsonValue::Kind::array;
                return array(out, depth);
            case '{':
                out.kind = JsonValue::Kind::object;
                return object(out, depth);
            default:
                out.kind = JsonValue::Kind::number;
                return number(out);
        }
    }

    bool array(JsonValue& out, int depth) {
        ++pos_;
        skip_space();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            JsonValue item;
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skip_space();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') return false;
            ++pos_;
        }
    }

    bool object(JsonValue& out, int depth) {
        ++pos_;
        skip_space();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_space();
            std::string key;
            if (pos_ >= s_.size() || s_[pos_] != '"' || !string(key)) return false;
            skip_space();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            JsonValue item;
            if (!value(item, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(item));
            skip_space();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') return false;
            ++pos_;
        }
    }

    bool hex4(char32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<char32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<char32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<char32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void append_utf8(char32_t c, std::string& out) {
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

    // Raw bytes are passed through (the display layer cleans them); escapes are decoded, and a
    // lone surrogate becomes U+FFFD.
    bool string(std::string& out) {
        ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            char e = s_[pos_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    char32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                        auto save = pos_;
                        pos_ += 2;
                        char32_t low = 0;
                        if (!hex4(low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = save;
                        }
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                    append_utf8(cp, out);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool number(JsonValue& out) {
        const auto start = pos_;
        auto digits = [&] {
            auto from = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            return pos_ > from;
        };
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        const auto int_start = pos_;
        if (!digits()) return false;
        if (s_[int_start] == '0' && pos_ - int_start > 1) return false;
        bool integral = true;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digits()) return false;
            integral = false;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!digits()) return false;
            integral = false;
        }
        out.integer = 0;
        if (integral) {
            auto text = s_.substr(start, pos_ - start);
            auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), out.integer);
            if (err != std::errc{} || end != text.data() + text.size()) out.integer = 0;
        }
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const {
    if (kind != Kind::object) return nullptr;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

std::optional<JsonValue> parse_json(std::string_view text) {
    if (text.size() > kMaxInput) return std::nullopt;
    return Parser(text).document();
}

}  // namespace ghostdrive::app
