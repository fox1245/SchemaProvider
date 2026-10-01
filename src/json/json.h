#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>

namespace sp::json {

class Members;
class Elements;
// Borrowed immutable view: owning Document must outlive every view and iterator.
class Value {
public:
    Value() = default;
    bool valid() const;
    bool is_null() const;
    bool is_bool() const;
    bool is_string() const;
    bool is_number() const;
    bool is_int() const;
    bool is_uint() const;
    bool is_array() const;
    bool is_object() const;
    std::string_view as_string() const;
    bool as_bool() const;
    std::int64_t as_int() const;
    std::uint64_t as_uint() const;
    double as_double() const;
    Value get(std::string_view key) const;
    Value at(std::size_t index) const;
    std::size_t size() const;
    Members members() const;
    Elements elements() const;
    std::string dump() const;
private:
    explicit Value(void* value) : value_(value) {}
    void* value_ = nullptr;
    friend class Document;
    friend class Members;
    friend class Elements;
    friend bool equal(Value, Value);
};

struct Member { std::string_view key; Value value; };
class Members {
public:
    class Iterator {
    public:
        using value_type = Member;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        Iterator() = default;
        Member operator*() const;
        Iterator& operator++();
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator&) const = default;
    private:
        Iterator(void* current, std::size_t remaining) : current_(current), remaining_(remaining) {}
        void* current_ = nullptr;
        std::size_t remaining_ = 0;
        friend class Members;
    };
    Iterator begin() const;
    Iterator end() const { return {}; }
private:
    explicit Members(Value value) : value_(value) {}
    Value value_;
    friend class Value;
};
class Elements {
public:
    class Iterator {
    public:
        using value_type = Value;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        Iterator() = default;
        Value operator*() const;
        Iterator& operator++();
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator&) const = default;
    private:
        Iterator(void* current, std::size_t remaining) : current_(current), remaining_(remaining) {}
        void* current_ = nullptr;
        std::size_t remaining_ = 0;
        friend class Elements;
    };
    Iterator begin() const;
    Iterator end() const { return {}; }
private:
    explicit Elements(Value value) : value_(value) {}
    Value value_;
    friend class Value;
};

struct Limits { std::size_t max_bytes = 1 << 20; std::size_t max_depth = 64; };
enum class ParseCode { Syntax, DuplicateKey, DepthExceeded, SizeExceeded };
struct ParseError;
class Document {
public:
    Document() = default;
    ~Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Value root() const;
private:
    explicit Document(void* document) : document_(document) {}
    void* document_ = nullptr;
    friend std::variant<Document, ParseError> parse(std::string_view, Limits);
};
struct ParseError {
    ParseCode code;
    std::string pointer;
    std::string message;
    // Diagnostic-only ownership of syntactically valid input rejected for duplicate
    // keys. Empty for other failures; this is not an accepted Document result.
    Document context{};
};
using ParseResult = std::variant<Document, ParseError>;
ParseResult parse(std::string_view input, Limits limits = {});
bool equal(Value lhs, Value rhs);
// Invalid UTF-8 returns an empty result; an empty input returns the JSON string "\"\"".
std::string quote(std::string_view input);

} // namespace sp::json
