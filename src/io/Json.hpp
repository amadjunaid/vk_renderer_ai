#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// A minimal, read-only JSON reader.
///
/// It exists because a glTF file is JSON wrapped in a binary container, and the
/// container is the only part of the format the loader can parse bytewise. Only
/// what reading a glTF needs is implemented - values, objects, arrays and a
/// handful of accessors - and nothing is written back out.
///
/// Values are stored by value in a tree: `object_` keeps the members in file
/// order, and lookups are a linear scan. glTF documents have few keys per
/// object, so a hash map would cost more than it saves.
namespace json {

/// Raised for malformed input, with the byte offset that gave up.
class ParseError : public std::runtime_error {
public:
    ParseError(std::string message, std::size_t offset);
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    Type type() const { return type_; }

    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    /// True when the value is a number, so a caller can tell "0" from "absent"
    /// without guessing. The typed accessors below are only meaningful on a
    /// value of the matching type; each falls back to its argument otherwise.
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double number(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
    float floatValue(float fallback = 0.0f) const {
        return type_ == Type::Number ? static_cast<float>(number_) : fallback;
    }
    int intValue(int fallback = 0) const {
        return type_ == Type::Number ? static_cast<int>(number_) : fallback;
    }
    const std::string& string() const { return string_; }

    /// Array length; 0 for a value that is not an array.
    std::size_t size() const { return type_ == Type::Array ? array_.size() : 0; }

    /// Array element. Out-of-range indices return the shared null value, so a
    /// truncated document reads as "missing" rather than crashing.
    const Value& at(std::size_t index) const;

    /// Object member, or the shared null value when the key is absent.
    const Value& at(std::string_view key) const;

    /// Copies up to `count` numbers out of an array of numbers. A shorter (or
    /// non-numeric) array leaves the remaining entries untouched, which lets
    /// callers pre-fill defaults and then overwrite.
    void numbers(float* out, std::size_t count) const;

    /// The shared null value, returned by the lookups above.
    static const Value& null();

private:
    friend class Parser;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

/// Parses a complete document. Trailing whitespace is allowed, anything else
/// after the top-level value is an error.
Value parse(std::string_view text);

}  // namespace json
