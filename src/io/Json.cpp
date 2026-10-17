#include "io/Json.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace json {

namespace {

/// The number of bytes a number token may occupy before it is copied into a
/// stack buffer. glTF values are small, so the heap path below is only a
/// safety net for hand-written documents.
constexpr std::size_t kNumberBufferSize = 64;

/// Renders `value` as the UTF-8 bytes for a Unicode code point.
void appendUtf8(std::string& out, std::uint32_t codePoint) {
    if (codePoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

}  // namespace

ParseError::ParseError(std::string message, std::size_t offset)
    : std::runtime_error(std::move(message) + " (at byte " + std::to_string(offset) + ")") {}

const Value& Value::null() {
    static const Value instance;
    return instance;
}

const Value& Value::at(std::size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) {
        return null();
    }
    return array_[index];
}

const Value& Value::at(std::string_view key) const {
    if (type_ != Type::Object) {
        return null();
    }

    for (const auto& member : object_) {
        if (member.first == key) {
            return member.second;
        }
    }
    return null();
}

void Value::numbers(float* out, std::size_t count) const {
    if (type_ != Type::Array) {
        return;
    }

    const std::size_t taken = count < array_.size() ? count : array_.size();
    for (std::size_t i = 0; i < taken; ++i) {
        if (array_[i].isNumber()) {
            out[i] = array_[i].floatValue();
        }
    }
}

/// A recursive-descent reader over one buffer. The parser only walks forward,
/// so offsets are all that is needed to report where something went wrong.
///
/// It lives at namespace scope rather than in an anonymous namespace because
/// Value names it in a friend declaration: an anonymous-namespace class would
/// be a *different* type and would not be able to touch the private members.
/// Nothing declares it in the header, so it stays private to this file.
class Parser {
public:
    explicit Parser(std::string_view text) : begin_(text.data()), cursor_(text.data()), end_(text.data() + text.size()) {}

    Value parseDocument() {
        Value root = parseValue();
        skipWhitespace();
        if (cursor_ != end_) {
            fail("unexpected trailing characters");
        }
        return root;
    }

private:
    // --- Cursor helpers ----------------------------------------------------

    bool atEnd() const { return cursor_ == end_; }

    char peek() const { return atEnd() ? '\0' : *cursor_; }

    std::size_t offset() const { return static_cast<std::size_t>(cursor_ - begin_); }

    [[noreturn]] void fail(const std::string& what) const {
        throw ParseError("JSON parse error: " + what, offset());
    }

    void skipWhitespace() {
        while (!atEnd()) {
            const char c = *cursor_;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++cursor_;
            } else {
                break;
            }
        }
    }

    void expect(char expected) {
        if (peek() != expected) {
            fail(std::string("expected '") + expected + "'");
        }
        ++cursor_;
    }

    void expectWord(const char* word) {
        for (const char* p = word; *p != '\0'; ++p) {
            if (peek() != *p) {
                fail(std::string("expected '") + word + "'");
            }
            ++cursor_;
        }
    }

    // --- Values ------------------------------------------------------------

    Value parseValue() {
        skipWhitespace();
        if (atEnd()) {
            fail("unexpected end of input");
        }

        switch (peek()) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': {
                Value value;
                value.type_ = Value::Type::String;
                value.string_ = parseString();
                return value;
            }
            case 't': {
                expectWord("true");
                Value value;
                value.type_ = Value::Type::Bool;
                value.bool_ = true;
                return value;
            }
            case 'f': {
                expectWord("false");
                Value value;
                value.type_ = Value::Type::Bool;
                value.bool_ = false;
                return value;
            }
            case 'n': {
                expectWord("null");
                return Value{};
            }
            default: return parseNumber();
        }
    }

    Value parseObject() {
        expect('{');

        Value value;
        value.type_ = Value::Type::Object;

        skipWhitespace();
        if (peek() == '}') {
            ++cursor_;
            return value;
        }

        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                fail("expected a member name");
            }
            std::string key = parseString();

            skipWhitespace();
            expect(':');

            value.object_.emplace_back(std::move(key), parseValue());

            skipWhitespace();
            if (peek() == ',') {
                ++cursor_;
                continue;
            }
            expect('}');
            return value;
        }
    }

    Value parseArray() {
        expect('[');

        Value value;
        value.type_ = Value::Type::Array;

        skipWhitespace();
        if (peek() == ']') {
            ++cursor_;
            return value;
        }

        while (true) {
            value.array_.push_back(parseValue());

            skipWhitespace();
            if (peek() == ',') {
                ++cursor_;
                continue;
            }
            expect(']');
            return value;
        }
    }

    std::string parseString() {
        expect('"');

        std::string result;
        while (true) {
            if (atEnd()) {
                fail("unterminated string");
            }

            const char c = *cursor_++;
            if (c == '"') {
                return result;
            }

            if (c != '\\') {
                result.push_back(c);
                continue;
            }

            if (atEnd()) {
                fail("unterminated escape sequence");
            }

            const char escape = *cursor_++;
            switch (escape) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codePoint = parseHex4();
                    // A code point above the basic multilingual plane is sent as
                    // a surrogate pair; recombine it so the string stays valid
                    // UTF-8 instead of holding two broken halves.
                    if (codePoint >= 0xD800u && codePoint <= 0xDBFFu && end_ - cursor_ >= 6 &&
                        cursor_[0] == '\\' && cursor_[1] == 'u') {
                        cursor_ += 2;
                        const std::uint32_t low = parseHex4();
                        if (low >= 0xDC00u && low <= 0xDFFFu) {
                            codePoint = 0x10000u + ((codePoint - 0xD800u) << 10) + (low - 0xDC00u);
                        } else {
                            fail("invalid low surrogate");
                        }
                    }
                    appendUtf8(result, codePoint);
                    break;
                }
                default: fail("unknown escape sequence");
            }
        }
    }

    std::uint32_t parseHex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (atEnd()) {
                fail("truncated \\u escape");
            }

            const char c = *cursor_++;
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid hex digit in \\u escape");
            }
            value = value * 16u + digit;
        }
        return value;
    }

    Value parseNumber() {
        const char* start = cursor_;

        if (peek() == '-') {
            ++cursor_;
        }
        while (!atEnd() && *cursor_ >= '0' && *cursor_ <= '9') {
            ++cursor_;
        }
        if (peek() == '.') {
            ++cursor_;
            while (!atEnd() && *cursor_ >= '0' && *cursor_ <= '9') {
                ++cursor_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++cursor_;
            if (peek() == '+' || peek() == '-') {
                ++cursor_;
            }
            while (!atEnd() && *cursor_ >= '0' && *cursor_ <= '9') {
                ++cursor_;
            }
        }

        if (cursor_ == start) {
            fail("expected a value");
        }

        // strtod wants a NUL-terminated string, and the token is not one: copy
        // it out first (with a stack buffer for the usual case).
        const std::size_t length = static_cast<std::size_t>(cursor_ - start);
        char local[kNumberBufferSize];
        std::string heap;
        const char* text = start;
        if (length < sizeof(local)) {
            std::memcpy(local, start, length);
            local[length] = '\0';
            text = local;
        } else {
            heap.assign(start, length);
            text = heap.c_str();
        }

        Value value;
        value.type_ = Value::Type::Number;
        value.number_ = std::strtod(text, nullptr);
        return value;
    }

    const char* begin_;
    const char* cursor_;
    const char* end_;
};

Value parse(std::string_view text) {
    return Parser(text).parseDocument();
}

}  // namespace json
