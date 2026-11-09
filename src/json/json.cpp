#include "json/json.h"
#include "crypto/crypto.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <limits>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sp::json {
namespace {
yyjson_val* val(void* pointer) { return static_cast<yyjson_val*>(pointer); }
// Store allocation extents in the same malloc/realloc block: no second
// allocation, global ledger, DOM traversal or Document ABI/layout change.
struct alignas(std::max_align_t) AllocationExtent { std::size_t bytes; };
void* document_malloc(void*, std::size_t size) noexcept {
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationExtent)) return nullptr;
    auto* allocation = static_cast<AllocationExtent*>(std::malloc(size + sizeof(AllocationExtent)));
    if (!allocation) return nullptr;
    allocation->bytes = size + sizeof(AllocationExtent);
    return allocation + 1;
}
void document_free(void*, void* pointer) noexcept {
    if (pointer) std::free(static_cast<AllocationExtent*>(pointer) - 1);
}
void* document_realloc(void*, void* pointer, std::size_t, std::size_t size) noexcept {
    if (!pointer) return document_malloc(nullptr, size);
    if (size == 0) { document_free(nullptr, pointer); return nullptr; }
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationExtent)) return nullptr;
    auto* allocation = static_cast<AllocationExtent*>(std::realloc(
        static_cast<AllocationExtent*>(pointer) - 1, size + sizeof(AllocationExtent)));
    if (!allocation) return nullptr;
    allocation->bytes = size + sizeof(AllocationExtent);
    return allocation + 1;
}
std::size_t allocation_extent(void* pointer) noexcept {
    return pointer ? (static_cast<AllocationExtent*>(pointer) - 1)->bytes : 0;
}
const yyjson_alc document_allocator{document_malloc, document_realloc, document_free, nullptr};
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
std::size_t Document::retained_bytes() const noexcept {
    auto* document = static_cast<yyjson_doc*>(document_);
    if (!document) return 0;
    if (document->alc.free != document_free) return std::numeric_limits<std::size_t>::max();
    const auto values = allocation_extent(document);
    const auto strings = allocation_extent(document->str_pool);
    if (values > std::numeric_limits<std::size_t>::max() - sizeof(Document) ||
        strings > std::numeric_limits<std::size_t>::max() - sizeof(Document) - values)
        return std::numeric_limits<std::size_t>::max();
    return values + strings + sizeof(Document);
}
std::size_t retained_size_bound(std::size_t source_bytes) noexcept {
    const auto bound = yyjson_read_max_memory_usage(source_bytes, 0);
    constexpr auto metadata = 2 * sizeof(AllocationExtent) + sizeof(Document);
    if (!bound || bound > std::numeric_limits<std::size_t>::max() - metadata)
        return std::numeric_limits<std::size_t>::max();
    return bound + metadata;
}
std::uint32_t retained_size_contract() noexcept { return 1; }
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
    auto* raw = yyjson_read_opts(const_cast<char*>(input.data()), input.size(), 0, &document_allocator, &error);
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

namespace {

bool valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const unsigned char b0 = static_cast<unsigned char>(text[i]);
        if (b0 <= 0x7f) {
            ++i;
            continue;
        }
        if (b0 >= 0xc2 && b0 <= 0xdf) {
            if (i + 1 >= n) return false;
            const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b1 & 0xc0) != 0x80) return false;
            i += 2;
            continue;
        }
        if (b0 >= 0xe0 && b0 <= 0xef) {
            if (i + 2 >= n) return false;
            const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
            if ((b1 & 0xc0) != 0x80 || (b2 & 0xc0) != 0x80) return false;
            if (b0 == 0xe0 && b1 < 0xa0) return false;
            if (b0 == 0xed && b1 >= 0xa0) return false;
            i += 3;
            continue;
        }
        if (b0 >= 0xf0 && b0 <= 0xf4) {
            if (i + 3 >= n) return false;
            const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
            const unsigned char b3 = static_cast<unsigned char>(text[i + 3]);
            if ((b1 & 0xc0) != 0x80 || (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80) return false;
            if (b0 == 0xf0 && b1 < 0x90) return false;
            if (b0 == 0xf4 && b1 >= 0x90) return false;
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

void BoundedWriter::wipe_temporary(std::string& temporary) const {
    if (wipe_temporaries_) crypto::cleanse(temporary.data(), temporary.size());
}
bool BoundedWriter::claim(std::size_t bytes) {
    if (!ok_ || bytes > limits_.max_bytes - size_) return ok_ = false;
    size_ += bytes;
    return true;
}
BoundedWriter& BoundedWriter::raw(std::string_view text) {
    if (claim(text.size())) emit(text);
    return *this;
}
BoundedWriter& BoundedWriter::quoted(std::string_view text) {
    if (!claim(2)) return *this;
    // Count the exact default yyjson escaping before creating any encoded data.
    // UTF-8 is checked when emitting the bounded chunks below.
    for (const unsigned char c : text) {
        const std::size_t bytes = c == '"' || c == '\\' || c == '\b' ||
            c == '\f' || c == '\n' || c == '\r' || c == '\t' ? 2 : c < 0x20 ? 6 : 1;
        if (!claim(bytes)) return *this;
    }
    if (!has_output()) return *this;
    emit("\"");
    while (!text.empty()) {
        auto length = std::min<std::size_t>(text.size(), 4096);
        // Never split a valid UTF-8 code point. Malformed sequences are not
        // repaired: both paths reject them, including overlong/surrogate encodings.
        if (length < text.size()) {
            while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
            if (!length) { ok_ = false; return *this; }
        }
        if (sink_) {
            const auto sub = text.substr(0, length);
            if (!valid_utf8(sub)) { ok_ = false; return *this; }
            std::size_t start = 0;
            for (std::size_t i = 0; i < sub.size(); ++i) {
                const auto c = static_cast<unsigned char>(sub[i]);
                if (c >= 0x20 && c != '"' && c != '\\') continue;
                emit(sub.substr(start, i - start));
                char escaped[6]{'\\', 'u', '0', '0', '0', '0'};
                std::size_t esc_len = 2;
                switch (c) {
                    case '"': escaped[1] = '"'; break;
                    case '\\': escaped[1] = '\\'; break;
                    case '\b': escaped[1] = 'b'; break;
                    case '\f': escaped[1] = 'f'; break;
                    case '\n': escaped[1] = 'n'; break;
                    case '\r': escaped[1] = 'r'; break;
                    case '\t': escaped[1] = 't'; break;
                    default: {
                        // The same upper-case hex digits yyjson writes, so both forms emit identical bytes.
                        constexpr char hex[] = "0123456789ABCDEF";
                        escaped[4] = hex[c >> 4];
                        escaped[5] = hex[c & 15];
                        esc_len = 6;
                        break;
                    }
                }
                emit({escaped, esc_len});
                start = i + 1;
            }
            emit(sub.substr(start));
        } else {
            auto chunk = quote(text.substr(0, length));
            if (chunk.empty()) { ok_ = false; return *this; }
            emit(std::string_view(chunk.data() + 1, chunk.size() - 2));
        }
        text.remove_prefix(length);
    }
    emit("\"");
    return *this;
}
BoundedWriter& BoundedWriter::value(Value item, std::size_t enclosing_depth) {
    if (!ok_) return *this;
    if (!item.valid()) { ok_ = false; return *this; }
    if (item.is_string()) return quoted(item.as_string());
    if (!item.is_object() && !item.is_array()) {
        // Only bounded scalar spellings reach yyjson's allocating serializer,
        // retaining its exact integer/real formatting and finite-number rules.
        auto scalar = item.dump();
        if (scalar.empty()) { ok_ = false; return *this; }
        raw(scalar);
        wipe_temporary(scalar);
        return *this;
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

BoundedWriter& BoundedWriter::quoted_json(Value item) {
    if (!ok_) return *this;
    // Document strings are already valid UTF-8. Escape their JSON spelling
    // once more for the enclosing string, copying ordinary runs directly.
    auto string = [&](std::string_view text) {
        raw("\\\"");
        std::size_t start = 0;
        for (std::size_t i = 0; ok_ && i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c >= 0x20 && c != '"' && c != '\\') continue;
            raw(text.substr(start, i - start));
            char escaped[7]{'\\', '\\', 'u', '0', '0', '0', '0'};
            std::size_t size = 3;
            switch (c) {
                case '"': case '\\': escaped[2] = '\\'; escaped[3] = static_cast<char>(c); size = 4; break;
                case '\b': escaped[2] = 'b'; break;
                case '\f': escaped[2] = 'f'; break;
                case '\n': escaped[2] = 'n'; break;
                case '\r': escaped[2] = 'r'; break;
                case '\t': escaped[2] = 't'; break;
                default: {
                    constexpr char hex[] = "0123456789abcdef";
                    escaped[5] = hex[c >> 4]; escaped[6] = hex[c & 15]; size = 7;
                }
            }
            raw({escaped, size});
            start = i + 1;
        }
        if (ok_) raw(text.substr(start)).raw("\\\"");
    };
    auto emit = [&](auto&& self, Value value, std::size_t depth) -> void {
        if (!ok_) return;
        if (!value.valid()) { ok_ = false; return; }
        if (value.is_string()) { string(value.as_string()); return; }
        if (!value.is_object() && !value.is_array()) {
            const auto scalar = value.dump();
            if (scalar.empty()) { ok_ = false; return; }
            raw(scalar);
            return;
        }
        if (depth >= limits_.max_depth) { ok_ = false; return; }
        bool comma = false;
        raw(value.is_object() ? "{" : "[");
        if (value.is_object()) {
            for (auto member : value.members()) {
                if (comma) raw(",");
                comma = true;
                string(member.key); raw(":"); self(self, member.value, depth + 1);
                if (!ok_) return;
            }
        } else {
            for (auto element : value.elements()) {
                if (comma) raw(",");
                comma = true;
                self(self, element, depth + 1);
                if (!ok_) return;
            }
        }
        raw(value.is_object() ? "}" : "]");
    };
    raw("\"");
    emit(emit, item, 0);
    return raw("\"");
}

} // namespace sp::json
