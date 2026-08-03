#include "json_lite.hpp"

#include <charconv>
#include <cctype>
#include <stdexcept>

namespace sui_pow::json_lite {
namespace {

class Cursor {
public:
    explicit Cursor(std::string_view input) : input_(input) {}

    std::size_t position() const { return position_; }
    bool done() const { return position_ >= input_.size(); }
    char peek() const {
        if (done()) {
            throw std::runtime_error("unexpected end of JSON");
        }
        return input_[position_];
    }
    char take() {
        const auto value = peek();
        ++position_;
        return value;
    }
    void expect(char expected) {
        if (take() != expected) {
            throw std::runtime_error("invalid JSON structure");
        }
    }
    void whitespace() {
        while (!done() && std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    std::string parse_string() {
        const auto start = position_;
        skip_string();
        return decode_string(input_.substr(start, position_ - start));
    }

    void skip_string() {
        expect('"');
        bool escaped = false;
        while (!done()) {
            const char value = take();
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                return;
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    void skip_value() {
        whitespace();
        switch (peek()) {
            case '"':
                skip_string();
                return;
            case '{':
                skip_object();
                return;
            case '[':
                skip_array();
                return;
            default:
                while (!done()) {
                    const char value = input_[position_];
                    if (value == ',' || value == '}' || value == ']' ||
                        std::isspace(static_cast<unsigned char>(value))) {
                        break;
                    }
                    ++position_;
                }
                return;
        }
    }

private:
    void skip_object() {
        expect('{');
        whitespace();
        if (!done() && peek() == '}') {
            take();
            return;
        }
        while (true) {
            whitespace();
            skip_string();
            whitespace();
            expect(':');
            skip_value();
            whitespace();
            const char separator = take();
            if (separator == '}') {
                return;
            }
            if (separator != ',') {
                throw std::runtime_error("invalid JSON object");
            }
        }
    }

    void skip_array() {
        expect('[');
        whitespace();
        if (!done() && peek() == ']') {
            take();
            return;
        }
        while (true) {
            skip_value();
            whitespace();
            const char separator = take();
            if (separator == ']') {
                return;
            }
            if (separator != ',') {
                throw std::runtime_error("invalid JSON array");
            }
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

unsigned int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned int>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned int>(value - 'A' + 10);
    }
    throw std::runtime_error("invalid JSON unicode escape");
}

unsigned int parse_hex4(std::string_view input, std::size_t& position) {
    if (position + 4 > input.size()) {
        throw std::runtime_error("short JSON unicode escape");
    }
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 4) | hex_digit(input[position++]);
    }
    return value;
}

void append_utf8(std::string& output, unsigned int codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

}  // namespace

std::string_view find_field(std::string_view object, std::string_view key) {
    Cursor cursor(object);
    cursor.whitespace();
    cursor.expect('{');
    cursor.whitespace();
    if (!cursor.done() && cursor.peek() == '}') {
        throw std::runtime_error("JSON field not found: " + std::string(key));
    }
    while (true) {
        cursor.whitespace();
        const auto current_key = cursor.parse_string();
        cursor.whitespace();
        cursor.expect(':');
        cursor.whitespace();
        const auto start = cursor.position();
        cursor.skip_value();
        const auto end = cursor.position();
        if (current_key == key) {
            return object.substr(start, end - start);
        }
        cursor.whitespace();
        const char separator = cursor.take();
        if (separator == '}') {
            break;
        }
        if (separator != ',') {
            throw std::runtime_error("invalid JSON object");
        }
    }
    throw std::runtime_error("JSON field not found: " + std::string(key));
}

std::string decode_string(std::string_view value) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        throw std::runtime_error("expected JSON string");
    }
    std::string output;
    output.reserve(value.size() - 2);
    for (std::size_t i = 1; i + 1 < value.size();) {
        const char current = value[i++];
        if (current != '\\') {
            output.push_back(current);
            continue;
        }
        if (i + 1 > value.size()) {
            throw std::runtime_error("bad JSON escape");
        }
        const char escaped = value[i++];
        switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned int codepoint = parse_hex4(value, i);
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (i + 6 > value.size() || value[i] != '\\' || value[i + 1] != 'u') {
                        throw std::runtime_error("bad JSON surrogate pair");
                    }
                    i += 2;
                    const unsigned int low = parse_hex4(value, i);
                    if (low < 0xdc00 || low > 0xdfff) {
                        throw std::runtime_error("bad JSON surrogate pair");
                    }
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                }
                append_utf8(output, codepoint);
                break;
            }
            default:
                throw std::runtime_error("unsupported JSON escape");
        }
    }
    return output;
}

std::string escape_string(std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(text.size() + 2);
    output.push_back('"');
    for (const unsigned char value : text) {
        switch (value) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (value < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[value >> 4]);
                    output.push_back(hex[value & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(value));
                }
        }
    }
    output.push_back('"');
    return output;
}

std::uint64_t parse_u64(std::string_view value) {
    std::string storage;
    if (!value.empty() && value.front() == '"') {
        storage = decode_string(value);
        value = storage;
    }
    std::uint64_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("expected unsigned JSON integer");
    }
    return result;
}

}  // namespace sui_pow::json_lite

