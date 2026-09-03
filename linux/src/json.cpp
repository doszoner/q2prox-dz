#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

#include <cerrno>
#include <cctype>
#include <cstdlib>
namespace q2dz::json {

bool Value::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage); }
bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(storage); }
bool Value::is_number() const noexcept { return std::holds_alternative<double>(storage); }
bool Value::is_string() const noexcept { return std::holds_alternative<std::string>(storage); }
bool Value::is_array() const noexcept { return std::holds_alternative<Array>(storage); }
bool Value::is_object() const noexcept { return std::holds_alternative<Object>(storage); }
const bool* Value::as_bool() const noexcept { return std::get_if<bool>(&storage); }
const std::string* Value::as_string() const noexcept { return std::get_if<std::string>(&storage); }
const Array* Value::as_array() const noexcept { return std::get_if<Array>(&storage); }
const Object* Value::as_object() const noexcept { return std::get_if<Object>(&storage); }

class Parser {
public:
    Parser(std::string_view input, std::size_t max_depth) : input_(input), max_depth_(max_depth) {}

    Result<Value> run() {
        skip_ws();
        auto value = parse_value(0);
        if (!value) return value;
        skip_ws();
        if (pos_ != input_.size()) return fail("trailing data");
        return value;
    }

private:
    Result<Value> fail(std::string message) const {
        return Result<Value>::fail("JSON at byte " + std::to_string(pos_) + ": " + std::move(message));
    }

    void skip_ws() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    Result<Value> parse_value(std::size_t depth) {
        if (depth > max_depth_) return fail("maximum nesting depth exceeded");
        skip_ws();
        if (pos_ >= input_.size()) return fail("unexpected end of input");
        const char c = input_[pos_];
        if (c == '{') return parse_object(depth + 1);
        if (c == '[') return parse_array(depth + 1);
        if (c == '"') {
            auto text = parse_string();
            if (!text) return Result<Value>::fail(text.error().message);
            return Result<Value>::ok(Value{std::move(text.value())});
        }
        if (c == 't' && consume("true")) return Result<Value>::ok(Value{true});
        if (c == 'f' && consume("false")) return Result<Value>::ok(Value{false});
        if (c == 'n' && consume("null")) return Result<Value>::ok(Value{nullptr});
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        return fail("unexpected token");
    }

    bool consume(std::string_view token) {
        if (input_.substr(pos_, token.size()) != token) return false;
        pos_ += token.size();
        return true;
    }

    Result<std::string> parse_string() {
        if (input_[pos_] != '"') return Result<std::string>::fail("expected string");
        ++pos_;
        std::string out;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return Result<std::string>::ok(std::move(out));
            if (c < 0x20U) return Result<std::string>::fail("control character in JSON string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) return Result<std::string>::fail("unterminated escape");
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto code = parse_hex4();
                    if (!code) return Result<std::string>::fail(code.error().message);
                    std::uint32_t cp = code.value();
                    if (cp >= 0xD800U && cp <= 0xDBFFU) {
                        if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                            return Result<std::string>::fail("unpaired high surrogate");
                        }
                        pos_ += 2;
                        auto low = parse_hex4();
                        if (!low || low.value() < 0xDC00U || low.value() > 0xDFFFU) {
                            return Result<std::string>::fail("invalid low surrogate");
                        }
                        cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low.value() - 0xDC00U);
                    } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                        return Result<std::string>::fail("unpaired low surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return Result<std::string>::fail("invalid JSON escape");
            }
        }
        return Result<std::string>::fail("unterminated string");
    }

    Result<std::uint32_t> parse_hex4() {
        if (pos_ + 4 > input_.size()) return Result<std::uint32_t>::fail("short unicode escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return Result<std::uint32_t>::fail("invalid unicode escape");
        }
        return Result<std::uint32_t>::ok(value);
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7FU) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FFU) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp <= 0xFFFFU) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }

    Result<Value> parse_number() {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) return fail("invalid number");
        if (input_[pos_] == '0') ++pos_;
        else {
            if (input_[pos_] < '1' || input_[pos_] > '9') return fail("invalid number");
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (digits == pos_) return fail("invalid fractional number");
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (digits == pos_) return fail("invalid exponent");
        }
        std::string token(input_.substr(start, pos_ - start));
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(token.c_str(), &end);
        if (errno != 0 || end != token.c_str() + token.size() || !std::isfinite(value)) return fail("number out of range");
        return Result<Value>::ok(Value{value});
    }

    Result<Value> parse_array(std::size_t depth) {
        ++pos_;
        Array array;
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == ']') { ++pos_; return Result<Value>::ok(Value{std::move(array)}); }
        while (true) {
            auto item = parse_value(depth);
            if (!item) return item;
            array.push_back(std::move(item.value()));
            skip_ws();
            if (pos_ >= input_.size()) return fail("unterminated array");
            if (input_[pos_] == ']') { ++pos_; break; }
            if (input_[pos_] != ',') return fail("expected comma in array");
            ++pos_;
        }
        return Result<Value>::ok(Value{std::move(array)});
    }

    Result<Value> parse_object(std::size_t depth) {
        ++pos_;
        Object object;
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == '}') { ++pos_; return Result<Value>::ok(Value{std::move(object)}); }
        while (true) {
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != '"') return fail("expected object key");
            auto key = parse_string();
            if (!key) return Result<Value>::fail(key.error().message);
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != ':') return fail("expected colon");
            ++pos_;
            auto value = parse_value(depth);
            if (!value) return value;
            if (!object.emplace(std::move(key.value()), std::move(value.value())).second) return fail("duplicate object key");
            skip_ws();
            if (pos_ >= input_.size()) return fail("unterminated object");
            if (input_[pos_] == '}') { ++pos_; break; }
            if (input_[pos_] != ',') return fail("expected comma in object");
            ++pos_;
        }
        return Result<Value>::ok(Value{std::move(object)});
    }

    std::string_view input_;
    std::size_t max_depth_;
    std::size_t pos_ = 0;
};

Result<Value> parse(std::string_view input, std::size_t max_depth) { return Parser(input, max_depth).run(); }

static void stringify_into(std::string& out, const Value& value) {
    if (value.is_null()) { out += "null"; return; }
    if (const auto* boolean = value.as_bool()) { out += *boolean ? "true" : "false"; return; }
    if (const auto* number = std::get_if<double>(&value.storage)) {
        char buffer[64]{};
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), *number, std::chars_format::general, 17);
        out.append(buffer, converted.ptr);
        return;
    }
    if (const auto* text = value.as_string()) {
        out.push_back('"');
        for (const char raw : *text) {
            const auto c = static_cast<unsigned char>(raw);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20U) {
                        char escaped[7]{};
                        std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                        out += escaped;
                    } else out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('"');
        return;
    }
    if (const auto* array = value.as_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto& item : *array) {
            if (!first) out.push_back(',');
            first = false;
            stringify_into(out, item);
        }
        out.push_back(']');
        return;
    }
    const auto* object = value.as_object();
    out.push_back('{');
    bool first = true;
    for (const auto& [key, item] : *object) {
        if (!first) out.push_back(',');
        first = false;
        stringify_into(out, Value{key});
        out.push_back(':');
        stringify_into(out, item);
    }
    out.push_back('}');
}

std::string stringify(const Value& value) { std::string out; stringify_into(out, value); return out; }

const Value* find_path(const Value& root, std::initializer_list<std::string_view> path) {
    const Value* current = &root;
    for (const auto part : path) {
        const auto* object = current->as_object();
        if (!object) return nullptr;
        const auto it = object->find(part);
        if (it == object->end()) return nullptr;
        current = &it->second;
    }
    return current;
}

const Value* find_first(const Value& root,
                        std::initializer_list<std::initializer_list<std::string_view>> paths) {
    for (const auto& path : paths) if (const auto* value = find_path(root, path)) return value;
    return nullptr;
}

static bool safe_alias(std::string_view alias) {
    const bool q2 = alias.size() > 3 && alias.starts_with("q2.");
    const bool q2ded = alias.size() > 11 && alias.starts_with("room.q2ded.");
    if (alias.size() > 255 || !(q2 || q2ded)) return false;
    for (const char raw : alias) {
        const auto c = static_cast<unsigned char>(raw);
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

Result<RoomConfig> extract_room_config(const Value& root) {
    RoomConfig out;
    const auto* alias = find_first(root, {{"alias"}, {"roomAlias"}, {"room", "alias"}, {"net", "alias"}});
    if (!alias || !alias->as_string() || !safe_alias(*alias->as_string())) {
        return Result<RoomConfig>::fail("room JSON must contain a safe q2.* or room.q2ded.* alias");
    }
    out.alias = *alias->as_string();

    const auto* ice = find_first(root, {{"iceServers"}, {"netConfig", "iceServers"}, {"room", "netConfig", "iceServers"}});
    if (!ice || !ice->is_array() || ice->as_array()->empty()) {
        return Result<RoomConfig>::fail("room JSON must contain a non-empty iceServers array");
    }
    out.ice_servers = *ice;

    if (const auto* ordered = find_first(root, {{"ordered"}, {"netConfig", "ordered"}, {"room", "netConfig", "ordered"}})) {
        if (!ordered->as_bool()) return Result<RoomConfig>::fail("ordered must be boolean");
        out.ordered = *ordered->as_bool();
        if (out.ordered) return Result<RoomConfig>::fail("Linux WebRTC transport requires ordered=false");
    }
    if (const auto* signaling = find_first(root, {{"signalingUrl"}, {"signaling", "url"}, {"netConfig", "signalingUrl"}})) {
        if (!signaling->as_string()) return Result<RoomConfig>::fail("signalingUrl must be string");
        out.signaling_url = *signaling->as_string();
    }
    if (const auto* map = find_first(root, {{"map"}, {"mapName"}, {"room", "map"}}); map && map->as_string()) out.map = *map->as_string();
    if (const auto* game = find_first(root, {{"game"}, {"gameDir"}, {"room", "game"}}); game && game->as_string()) out.game = *game->as_string();
    return Result<RoomConfig>::ok(std::move(out));
}

} // namespace q2dz::json
