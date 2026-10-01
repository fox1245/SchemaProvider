#include "json/json.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sp::json {
namespace {
yyjson_val* val(void* pointer) { return static_cast<yyjson_val*>(pointer); }
std::string pointer_segment(std::string_view key) {
    std::string result;
    for (char c : key) {
        if (c == '~') result += "~0";
        else if (c == '/') result += "~1";
        else result += c;
    }
    return result;
}
std::string serialize(yyjson_val* value) {
    if (!value) return {};
    std::size_t length = 0;
    yyjson_write_err error{};
    std::unique_ptr<char, decltype(&std::free)> text(
        yyjson_val_write_opts(value, 0, nullptr, &length, &error), &std::free);
    if (!text) {
        if (error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION) throw std::bad_alloc();
        return {};
    }
    return {text.get(), length};
}
// Explicit work stack avoids recursion even when callers increase max_depth.
std::optional<ParseError> duplicates(Value root) {
    if (!root.is_object() && !root.is_array()) return std::nullopt;
    struct Pending { Value value; std::string pointer; };
    std::vector<Pending> pending{{root, {}}};
    while (!pending.empty()) {
        auto item = std::move(pending.back());
        pending.pop_back();
        if (item.value.is_object()) {
            std::unordered_set<std::string_view> keys;
            keys.reserve(item.value.size());
            for (auto member : item.value.members()) {
                if (!keys.insert(member.key).second)
                    return ParseError{ParseCode::DuplicateKey,
                        item.pointer + "/" + pointer_segment(member.key), "duplicate object member"};
                if (member.value.is_object() || member.value.is_array())
                    pending.push_back({member.value, item.pointer + "/" + pointer_segment(member.key)});
            }
        } else if (item.value.is_array()) {
            std::size_t index = 0;
            for (auto element : item.value.elements()) {
                if (element.is_object() || element.is_array())
                    pending.push_back({element, item.pointer + "/" + std::to_string(index)});
                ++index;
            }
        }
    }
    return std::nullopt;
}
}

bool Value::valid() const { return value_ != nullptr; }
bool Value::is_null() const { return yyjson_is_null(val(value_)); }
bool Value::is_bool() const { return yyjson_is_bool(val(value_)); }
bool Value::is_string() const { return yyjson_is_str(val(value_)); }
bool Value::is_number() const { return yyjson_is_num(val(value_)); }
bool Value::is_int() const { return yyjson_is_int(val(value_)); }
bool Value::is_uint() const { return yyjson_is_uint(val(value_)); }
bool Value::is_array() const { return yyjson_is_arr(val(value_)); }
bool Value::is_object() const { return yyjson_is_obj(val(value_)); }
std::string_view Value::as_string() const {
    auto* string = yyjson_get_str(val(value_));
    return string ? std::string_view(string, yyjson_get_len(val(value_))) : std::string_view{};
}
bool Value::as_bool() const { return yyjson_get_bool(val(value_)); }
std::int64_t Value::as_int() const { return yyjson_get_sint(val(value_)); }
std::uint64_t Value::as_uint() const { return yyjson_get_uint(val(value_)); }
double Value::as_double() const { return yyjson_get_num(val(value_)); }
Value Value::get(std::string_view key) const { return Value(yyjson_obj_getn(val(value_), key.data(), key.size())); }
Value Value::at(std::size_t index) const { return Value(yyjson_arr_get(val(value_), index)); }
std::size_t Value::size() const {
    return is_object() ? yyjson_obj_size(val(value_)) : yyjson_arr_size(val(value_));
}
Members Value::members() const { return Members(*this); }
Elements Value::elements() const { return Elements(*this); }
std::string Value::dump() const { return serialize(val(value_)); }
Members::Iterator Members::begin() const {
    auto iter = yyjson_obj_iter_with(val(value_.value_));
    return iter.max ? Iterator(iter.cur, iter.max) : Iterator{};
}
Member Members::Iterator::operator*() const {
    auto* key = val(current_);
    return {{yyjson_get_str(key), yyjson_get_len(key)}, Value(yyjson_obj_iter_get_val(key))};
}
Members::Iterator& Members::Iterator::operator++() {
    if (remaining_) {
        yyjson_obj_iter iter{0, remaining_, val(current_), nullptr};
        yyjson_obj_iter_next(&iter);
        current_ = --remaining_ ? iter.cur : nullptr;
    }
    return *this;
}
Elements::Iterator Elements::begin() const {
    auto iter = yyjson_arr_iter_with(val(value_.value_));
    return iter.max ? Iterator(iter.cur, iter.max) : Iterator{};
}
Value Elements::Iterator::operator*() const { return Value(current_); }
Elements::Iterator& Elements::Iterator::operator++() {
    if (remaining_) {
        yyjson_arr_iter iter{0, remaining_, val(current_)};
        yyjson_arr_iter_next(&iter);
        current_ = --remaining_ ? iter.cur : nullptr;
    }
    return *this;
}
Document::~Document() { yyjson_doc_free(static_cast<yyjson_doc*>(document_)); }
Document::Document(Document&& other) noexcept : document_(std::exchange(other.document_, nullptr)) {}
Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) {
        yyjson_doc_free(static_cast<yyjson_doc*>(document_));
        document_ = std::exchange(other.document_, nullptr);
    }
    return *this;
}
Value Document::root() const { return Value(yyjson_doc_get_root(static_cast<yyjson_doc*>(document_))); }
ParseResult parse(std::string_view input, Limits limits) {
    if (input.size() > limits.max_bytes)
        return ParseError{ParseCode::SizeExceeded, {}, "JSON byte limit exceeded"};
    // This scan only bounds containers; yyjson remains the sole grammar parser.
    std::size_t depth = 0;
    bool string = false, escaped = false;
    for (char c : input) {
        if (string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') string = false;
        } else if (c == '"') string = true;
        else if (c == '{' || c == '[') {
            if (depth == limits.max_depth)
                return ParseError{ParseCode::DepthExceeded, {}, "JSON nesting limit exceeded"};
            ++depth;
        } else if ((c == '}' || c == ']') && depth) --depth;
    }
    yyjson_read_err error{};
    auto* raw = yyjson_read_opts(const_cast<char*>(input.data()), input.size(), 0, nullptr, &error);
    if (!raw) {
        if (error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) throw std::bad_alloc();
        return ParseError{ParseCode::Syntax, {}, "invalid JSON document"};
    }
    Document document(raw);
    if (auto error_value = duplicates(document.root())) {
        error_value->context = std::move(document);
        return std::move(*error_value);
    }
    return document;
}
bool equal(Value lhs, Value rhs) {
    if (!lhs.valid() || !rhs.valid()) return false;
    if (!lhs.is_object() && !lhs.is_array())
        return yyjson_equals(val(lhs.value_), val(rhs.value_));
    // Iterative structural comparison handles arbitrary configured depth safely.
    std::vector<std::pair<Value, Value>> pending{{lhs, rhs}};
    while (!pending.empty()) {
        auto [a, b] = pending.back();
        pending.pop_back();
        if (!a.valid() || !b.valid()) return false;
        if (a.is_object()) {
            if (!b.is_object() || a.size() != b.size()) return false;
            for (auto member : a.members()) {
                auto other = b.get(member.key);
                if (!other.valid()) return false;
                pending.emplace_back(member.value, other);
            }
        } else if (a.is_array()) {
            if (!b.is_array() || a.size() != b.size()) return false;
            auto other = b.elements().begin();
            for (auto element : a.elements()) pending.emplace_back(element, *other++);
        } else if (!yyjson_equals(val(a.value_), val(b.value_))) return false;
    }
    return true;
}
std::string quote(std::string_view input) {
    yyjson_val value{};
    yyjson_set_strn(&value, input.empty() ? "" : input.data(), input.size());
    return serialize(&value);
}

bool BoundedWriter::claim(std::size_t bytes) {
    if (!ok_ || bytes > limits_.max_bytes - size_) return ok_ = false;
    size_ += bytes;
    return true;
}
BoundedWriter& BoundedWriter::raw(std::string_view text) {
    if (claim(text.size()) && output_) output_->append(text);
    return *this;
}
BoundedWriter& BoundedWriter::quoted(std::string_view text) {
    if (!claim(2)) return *this;
    // Count the exact default yyjson escaping before creating any encoded data.
    // UTF-8 is checked by yyjson when emitting the bounded chunks below.
    for (const unsigned char c : text) {
        const std::size_t bytes = c == '"' || c == '\\' || c == '\b' ||
            c == '\f' || c == '\n' || c == '\r' || c == '\t' ? 2 : c < 0x20 ? 6 : 1;
        if (!claim(bytes)) return *this;
    }
    if (!output_) return *this;
    output_->push_back('"');
    while (!text.empty()) {
        auto length = std::min<std::size_t>(text.size(), 4096);
        // Never split a valid UTF-8 code point. Malformed sequences are not
        // repaired: quote rejects them, including overlong/surrogate encodings.
        if (length < text.size()) {
            while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
            if (!length) { ok_ = false; return *this; }
        }
        const auto chunk = quote(text.substr(0, length));
        if (chunk.empty()) { ok_ = false; return *this; }
        output_->append(chunk.data() + 1, chunk.size() - 2);
        text.remove_prefix(length);
    }
    output_->push_back('"');
    return *this;
}
BoundedWriter& BoundedWriter::value(Value item, std::size_t enclosing_depth) {
    if (!ok_) return *this;
    if (!item.valid()) { ok_ = false; return *this; }
    if (item.is_string()) return quoted(item.as_string());
    if (!item.is_object() && !item.is_array()) {
        // Only bounded scalar spellings reach yyjson's allocating serializer,
        // retaining its exact integer/real formatting and finite-number rules.
        const auto scalar = item.dump();
        if (scalar.empty()) { ok_ = false; return *this; }
        return raw(scalar);
    }
    if (enclosing_depth >= limits_.max_depth) { ok_ = false; return *this; }
    bool comma = false;
    raw(item.is_object() ? "{" : "[");
    if (item.is_object()) {
        for (auto member : item.members()) {
            if (comma) raw(",");
            comma = true;
            quoted(member.key).raw(":").value(member.value, enclosing_depth + 1);
            if (!ok_) return *this;
        }
    } else {
        for (auto element : item.elements()) {
            if (comma) raw(",");
            comma = true;
            value(element, enclosing_depth + 1);
            if (!ok_) return *this;
        }
    }
    return raw(item.is_object() ? "}" : "]");
}

} // namespace sp::json
