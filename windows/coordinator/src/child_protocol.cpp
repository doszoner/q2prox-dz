#include "q2dz/child_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace q2dz {
namespace {

constexpr std::string_view v1_prefix = "Q2DZ/1 ";
constexpr std::string_view rooms_prefix = "Q2DZ/ROOMS/2 ";
constexpr std::string_view hub_prefix = "Q2DZ/HUB/2 ";
constexpr std::size_t max_hub_line_bytes = 256U * 1024U;

constexpr std::string_view fatal_line(ChildEventFramer::Error error) noexcept {
    return error == ChildEventFramer::Error::line_too_long
        ? "Q2DZ/1 FATAL stage=protocol code=line_too_long\n"
        : "Q2DZ/1 FATAL stage=protocol code=queue_overflow\n";
}

constexpr std::size_t max_fatal_line_bytes =
    fatal_line(ChildEventFramer::Error::line_too_long).size() > fatal_line(ChildEventFramer::Error::queue_overflow).size()
        ? fatal_line(ChildEventFramer::Error::line_too_long).size()
        : fatal_line(ChildEventFramer::Error::queue_overflow).size();

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has_forbidden_control(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char c) {
        return c == '\0' || c == '\r' || c == '\n';
    });
}

bool valid_utf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7fU) {
            ++index;
            continue;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((length == 3 && codepoint < 0x800U) ||
            (length == 4 && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
        index += length;
    }
    return true;
}

bool safe_room_id(std::string_view value) {
    if (value.empty() || value.size() > 255U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_';
    });
}

bool safe_event_field_name(std::string_view value) {
    if (value.empty() || value.size() > 64U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

bool safe_event_field_value(std::string_view value) {
    if (value.empty() || value.size() > 1024U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x21U && c <= 0x7eU;
    });
}

bool parse_u64(std::string_view text, std::uint64_t& value, bool allow_zero = true) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && (allow_zero || value != 0);
}

void secure_clear_bytes(std::vector<unsigned char>& value) noexcept {
    volatile unsigned char* data = value.empty() ? nullptr : value.data();
    for (std::size_t index = 0; index < value.size(); ++index) data[index] = 0;
    value.clear();
    value.shrink_to_fit();
}

void secure_clear_text(std::string& value) noexcept {
    volatile char* data = value.empty() ? nullptr : value.data();
    for (std::size_t index = 0; index < value.size(); ++index) data[index] = 0;
    value.clear();
    value.shrink_to_fit();
}

class WipedText {
public:
    explicit WipedText(std::string value) : value_(value) { secure_clear_text(value); }
    ~WipedText() { secure_clear_text(value_); }
    [[nodiscard]] const std::string& get() const noexcept { return value_; }

private:
    std::string value_;
};

bool is_room_alias_value(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 2> prefixes = {"q2.", "room.q2ded."};
    for (const auto prefix : prefixes) {
        if (value.size() < prefix.size()) continue;
        for (std::size_t start = 0; start + prefix.size() <= value.size(); ++start) {
            bool match = true;
            for (std::size_t index = 0; index < prefix.size(); ++index) {
                if (std::tolower(static_cast<unsigned char>(value[start + index])) != prefix[index]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

bool allowed_hub_event_field(ChildEventType type, std::string_view name) noexcept {
    if (type == ChildEventType::transport_ready) return name == "id" || name == "udp" || name == "peer";
    if (type == ChildEventType::room_closed) return name == "id" || name == "reason";
    if (type == ChildEventType::heartbeat) return name == "state" || name == "targets" || name == "ready";
    if (type == ChildEventType::fatal || type == ChildEventType::target_failed) return name == "id" || name == "stage" || name == "code";
    return false;
}

class CompactJson {
public:
    explicit CompactJson(std::string_view input) : input_(input) {}

    bool parse(char root) {
        return !input_.empty() && input_.front() == root && value(0) && position_ == input_.size();
    }

private:
    bool value(std::size_t depth) {
        if (depth > 64U || position_ >= input_.size()) return false;
        switch (input_[position_]) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return string(nullptr);
            case 't': return literal("true");
            case 'f': return literal("false");
            case 'n': return literal("null");
            default: return number();
        }
    }

    bool object(std::size_t depth) {
        ++position_;
        std::set<std::string> keys;
        if (position_ < input_.size() && input_[position_] == '}') { ++position_; return true; }
        for (;;) {
            std::string key;
            if (!string(&key) || !keys.insert(std::move(key)).second || position_ >= input_.size() || input_[position_++] != ':' ||
                !value(depth + 1U)) return false;
            if (position_ >= input_.size()) return false;
            if (input_[position_] == '}') { ++position_; return true; }
            if (input_[position_++] != ',') return false;
        }
    }

    bool array(std::size_t depth) {
        ++position_;
        if (position_ < input_.size() && input_[position_] == ']') { ++position_; return true; }
        for (;;) {
            if (!value(depth + 1U) || position_ >= input_.size()) return false;
            if (input_[position_] == ']') { ++position_; return true; }
            if (input_[position_++] != ',') return false;
        }
    }

    bool string(std::string* decoded) {
        if (position_ >= input_.size() || input_[position_++] != '"') return false;
        while (position_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20U) return false;
            if (character != '\\') {
                if (decoded) decoded->push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            if (escaped == '"' || escaped == '\\' || escaped == '/') {
                if (decoded) decoded->push_back(escaped);
            } else if (escaped == 'b' || escaped == 'f' || escaped == 'n' || escaped == 'r' || escaped == 't') {
                if (decoded) decoded->push_back(escaped == 'b' ? '\b' : escaped == 'f' ? '\f' : escaped == 'n' ? '\n' : escaped == 'r' ? '\r' : '\t');
            } else if (escaped == 'u') {
                std::uint32_t codepoint = 0;
                if (!hex_codepoint(codepoint)) return false;
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (position_ + 6U > input_.size() || input_[position_] != '\\' || input_[position_ + 1U] != 'u') return false;
                    position_ += 2U;
                    std::uint32_t low = 0;
                    if (!hex_codepoint(low) || low < 0xdc00U || low > 0xdfffU) return false;
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    return false;
                }
                if (decoded) append_utf8(*decoded, codepoint);
            } else return false;
        }
        return false;
    }

    bool hex_codepoint(std::uint32_t& codepoint) {
        if (position_ + 4U > input_.size()) return false;
        codepoint = 0;
        for (std::size_t digit = 0; digit < 4U; ++digit) {
            const int value = hex_value(input_[position_ + digit]);
            if (value < 0) return false;
            codepoint = (codepoint << 4U) | static_cast<std::uint32_t>(value);
        }
        position_ += 4U;
        return true;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    bool number() {
        const auto start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') ++position_;
        else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        } else return false;
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') return false;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') return false;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        double parsed = 0.0;
        const auto result = std::from_chars(input_.data() + start, input_.data() + position_, parsed);
        return result.ec == std::errc{} && result.ptr == input_.data() + position_ && std::isfinite(parsed);
    }

    bool literal(std::string_view expected) {
        if (input_.substr(position_, expected.size()) != expected) return false;
        position_ += expected.size();
        return true;
    }

    std::string_view input_;
    std::size_t position_{0};
};

bool json_payload_ok(std::string_view value, char root) {
    return !value.empty() && valid_utf8(value) && !has_forbidden_control(value) && CompactJson(value).parse(root);
}

HubParseResult hub_failure(std::string message) {
    return HubParseResult{std::nullopt, std::move(message)};
}

HubParseResult hub_success(HubCommand command) {
    return HubParseResult{std::optional<HubCommand>(std::move(command)), {}};
}

HubParseResult parse_command(std::string_view line, HubCommandChannel channel, bool* ice_seen) {
    if (line.empty() || line.size() > max_hub_line_bytes || !valid_utf8(line) || has_forbidden_control(line)) {
        return hub_failure("hub command framing is invalid");
    }
    const auto prefix = channel == HubCommandChannel::rooms ? rooms_prefix : hub_prefix;
    if (!line.starts_with(prefix)) return hub_failure("unsupported hub protocol");
    line.remove_prefix(prefix.size());
    const auto command_end = line.find(' ');
    const auto command_name = line.substr(0, command_end);
    HubCommandType type{};
    if (command_name == "ICE") type = HubCommandType::ice;
    else if (command_name == "PUT") type = HubCommandType::put;
    else if (command_name == "DROP") type = HubCommandType::drop;
    else if (command_name == "STOP" && channel == HubCommandChannel::hub) {
        if (command_end != std::string_view::npos) return hub_failure("STOP takes no fields");
        HubCommand stop;
        stop.type = HubCommandType::stop;
        return hub_success(std::move(stop));
    } else return hub_failure("unknown hub command");
    if (command_end == std::string_view::npos) return hub_failure("hub command lacks fields");
    line.remove_prefix(command_end + 1U);

    HubCommand out;
    out.type = type;
    const bool room_channel = channel == HubCommandChannel::rooms;
    const std::string_view identity = type == HubCommandType::ice ? "generation=" : room_channel ? "room=" : "id=";
    if (type == HubCommandType::ice || type == HubCommandType::put) {
        const auto json_separator = line.find(" json=");
        if (json_separator == std::string_view::npos || json_separator < identity.size() || !line.starts_with(identity)) {
            return hub_failure("hub command requires fixed json= field");
        }
        const auto identity_value = line.substr(identity.size(), json_separator - identity.size());
        if (identity_value.empty()) return hub_failure("hub command identity field is empty");
        if (type == HubCommandType::ice) {
            if (!parse_u64(identity_value, out.id_or_generation, false)) return hub_failure("ICE generation is invalid");
        } else if (room_channel) {
            if (!safe_room_id(identity_value)) return hub_failure("room ID is unsafe");
            out.room_id = std::string(identity_value);
        } else if (!parse_u64(identity_value, out.id_or_generation, false)) {
            return hub_failure("target ID is invalid");
        }
        if (type == HubCommandType::put && ice_seen != nullptr && !*ice_seen) return hub_failure("PUT requires a preceding ICE command");
        const auto payload = line.substr(json_separator + 6U);
        if (payload.empty() || !valid_utf8(payload) || has_forbidden_control(payload) ||
            !json_payload_ok(payload, type == HubCommandType::ice ? '[' : '{')) {
            return hub_failure("hub JSON payload is invalid");
        }
        out.json.assign(payload.begin(), payload.end());
    } else {
        if (!line.starts_with(identity) || line.find(' ') != std::string_view::npos) return hub_failure("hub DROP field is invalid");
        const auto identity_value = line.substr(identity.size());
        if (room_channel) {
            if (!safe_room_id(identity_value)) return hub_failure("room ID is unsafe");
            out.room_id = std::string(identity_value);
        } else if (!parse_u64(identity_value, out.id_or_generation, false)) {
            return hub_failure("target ID is invalid");
        }
    }
    if (type == HubCommandType::ice && ice_seen != nullptr) *ice_seen = true;
    return hub_success(std::move(out));
}

std::string command_json(const HubCommand& command) {
    if (command.json.empty()) return {};
    return std::string(reinterpret_cast<const char*>(command.json.data()), command.json.size());
}

std::string format_command(HubCommandChannel channel, const HubCommand& command) {
    const auto prefix = channel == HubCommandChannel::rooms ? rooms_prefix : hub_prefix;
    std::string out(prefix);
    WipedText payload(command_json(command));
    switch (command.type) {
        case HubCommandType::ice:
            if (command.id_or_generation == 0 || !json_payload_ok(payload.get(), '[')) return {};
            out += "ICE generation=" + std::to_string(command.id_or_generation) + " json=";
            break;
        case HubCommandType::put:
            if (channel == HubCommandChannel::rooms) {
                if (!safe_room_id(command.room_id)) return {};
                out += "PUT room=" + command.room_id + " json=";
            } else {
                if (command.id_or_generation == 0) return {};
                out += "PUT id=" + std::to_string(command.id_or_generation) + " json=";
            }
            if (!json_payload_ok(payload.get(), '{')) return {};
            break;
        case HubCommandType::drop:
            if (channel == HubCommandChannel::rooms) {
                if (!safe_room_id(command.room_id)) return {};
                return out + "DROP room=" + command.room_id;
            }
            if (command.id_or_generation == 0) return {};
            return out + "DROP id=" + std::to_string(command.id_or_generation);
        case HubCommandType::stop:
            if (channel != HubCommandChannel::hub || command.id_or_generation != 0 || !command.room_id.empty() || !command.json.empty()) return {};
            return out + "STOP";
    }
    out += payload.get();
    return out.size() <= max_hub_line_bytes ? out : std::string{};
}

ParseResult parse_v1_event(std::string_view line) {
    if (line.size() > 4096U) return {{}, "event line exceeds 4096 bytes"};
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (!line.starts_with(v1_prefix)) return {{}, "missing Q2DZ/1 prefix"};
    line.remove_prefix(v1_prefix.size());
    const auto sp = line.find(' ');
    const auto type_text = line.substr(0, sp);
    ChildEvent e;
    if (type_text == "TRANSPORT_READY") e.type = ChildEventType::transport_ready;
    else if (type_text == "ROOM_CLOSED") e.type = ChildEventType::room_closed;
    else if (type_text == "FATAL") e.type = ChildEventType::fatal;
    else if (type_text == "HEARTBEAT") e.type = ChildEventType::heartbeat;
    else return {{}, "unknown child event"};
    if (sp != std::string_view::npos) line.remove_prefix(sp + 1U); else line = {};
    while (!line.empty()) {
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line.empty()) break;
        const auto next = line.find(' ');
        const auto token = line.substr(0, next);
        const auto eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1U >= token.size()) return {{}, "invalid key=value field"};
        const auto key = token.substr(0, eq);
        if (field_name_is_forbidden(key)) return {{}, "forbidden sensitive field in child event"};
        for (char c : key) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return {{}, "invalid field name"};
        bool ok = false;
        auto value = percent_decode(token.substr(eq + 1U), &ok);
        if (!ok) return {{}, "invalid percent-encoded value"};
        if (!e.fields.emplace(std::string(key), std::move(value)).second) return {{}, "duplicate field"};
        if (next == std::string_view::npos) break;
        line.remove_prefix(next + 1U);
    }
    if (e.type == ChildEventType::transport_ready) {
        const auto it = e.fields.find("udp");
        if (it == e.fields.end() || !parse_loopback_endpoint(it->second)) return {{}, "TRANSPORT_READY requires valid loopback udp field"};
    } else if (e.type == ChildEventType::fatal) {
        if (!e.fields.contains("stage") || !e.fields.contains("code")) return {{}, "FATAL requires stage and code fields"};
    }
    return {std::move(e), {}};
}

ParseResult parse_v2_event(std::string_view line) {
    if (line.empty() || line.size() > max_hub_line_bytes || !valid_utf8(line) || has_forbidden_control(line) || !line.starts_with(hub_prefix)) {
        return {{}, "invalid HUB/2 event framing"};
    }
    line.remove_prefix(hub_prefix.size());
    const auto sp = line.find(' ');
    const auto type_text = line.substr(0, sp);
    ChildEvent e;
    if (type_text == "READY") e.type = ChildEventType::transport_ready;
    else if (type_text == "CLOSED") e.type = ChildEventType::room_closed;
    else if (type_text == "FAILED") e.type = ChildEventType::target_failed;
    else if (type_text == "HEARTBEAT") e.type = ChildEventType::heartbeat;
    else return {{}, "unknown HUB/2 event"};
    if (sp != std::string_view::npos) line.remove_prefix(sp + 1U); else line = {};
    while (!line.empty()) {
        const auto next = line.find(' ');
        const auto token = line.substr(0, next);
        const auto eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1U >= token.size()) return {{}, "invalid HUB/2 event field"};
        const auto key = token.substr(0, eq);
        const auto value = token.substr(eq + 1U);
        if (!safe_event_field_name(key) || lower(key) == "alias" || !safe_event_field_value(value) ||
            is_room_alias_value(value) || field_name_is_forbidden(key) || field_name_is_forbidden(value) ||
            !allowed_hub_event_field(e.type, key)) {
            return {{}, "invalid or sensitive HUB/2 event field"};
        }
        if (!e.fields.emplace(std::string(key), std::string(value)).second) return {{}, "duplicate HUB/2 event field"};
        if (next == std::string_view::npos) break;
        line.remove_prefix(next + 1U);
    }
    if (e.type != ChildEventType::heartbeat) {
        const auto id = e.fields.find("id");
        std::uint64_t parsed = 0;
        if (id == e.fields.end() || !parse_u64(id->second, parsed, false)) return {{}, "HUB/2 event requires nonzero id"};
    }
    if (e.type == ChildEventType::transport_ready) {
        const auto udp = e.fields.find("udp");
        const auto peer = e.fields.find("peer");
        std::uint64_t parsed_peer = 0;
        if (udp == e.fields.end() || !parse_loopback_endpoint(udp->second) || peer == e.fields.end() || !parse_u64(peer->second, parsed_peer, false)) {
            return {{}, "READY requires loopback udp and numeric peer"};
        }
    } else if (e.type == ChildEventType::room_closed) {
        if (!e.fields.contains("reason")) return {{}, "CLOSED requires reason"};
    } else if (e.type == ChildEventType::target_failed) {
        if (!e.fields.contains("stage") || !e.fields.contains("code")) return {{}, "FAILED requires stage and code"};
    } else {
        const auto state = e.fields.find("state");
        const auto targets = e.fields.find("targets");
        const auto ready = e.fields.find("ready");
        std::uint64_t target_count = 0;
        std::uint64_t ready_count = 0;
        if (state == e.fields.end() || state->second != "ok" || targets == e.fields.end() || ready == e.fields.end() ||
            !parse_u64(targets->second, target_count) || target_count > 256U || !parse_u64(ready->second, ready_count) || ready_count > target_count) {
            return {{}, "HEARTBEAT counters are invalid"};
        }
    }
    return {std::move(e), {}};
}

void append_hub_event_fields(std::string& out,
                             ChildEventType type,
                             const std::map<std::string, std::string, std::less<>>& fields) {
    std::array<std::string_view, 4> order{};
    std::size_t count = 0;
    switch (type) {
        case ChildEventType::transport_ready: order = {"id", "udp", "peer", {}}; count = 3; break;
        case ChildEventType::room_closed: order = {"id", "reason", {}, {}}; count = 2; break;
        case ChildEventType::fatal: order = {"id", "stage", "code", {}}; count = 3; break;
        case ChildEventType::target_failed: order = {"id", "stage", "code", {}}; count = 3; break;
        case ChildEventType::heartbeat: order = {"state", "targets", "ready", {}}; count = 3; break;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (const auto it = fields.find(order[index]); it != fields.end()) {
            out += ' ' + std::string(order[index]);
            out.push_back('=');
            out += it->second;
        }
    }
    for (const auto& [key, value] : fields) {
        bool ordered = false;
        for (std::size_t index = 0; index < count; ++index) ordered = ordered || key == order[index];
        if (!ordered) {
            out.push_back(' ');
            out += key;
            out.push_back('=');
            out += value;
        }
    }
}

bool valid_hub_event_fields(ChildEventType type,
                            const std::map<std::string, std::string, std::less<>>& fields) {
    return std::all_of(fields.begin(), fields.end(), [type](const auto& field) {
        return safe_event_field_name(field.first) && lower(field.first) != "alias" &&
               allowed_hub_event_field(type, field.first) && safe_event_field_value(field.second) &&
               !is_room_alias_value(field.second) && !field_name_is_forbidden(field.first) &&
               !field_name_is_forbidden(field.second);
    });
}

} // namespace

WipedLine::WipedLine(std::string&& value) : value_(value) {
    secure_clear_text(value);
}

WipedLine::WipedLine(WipedLine&& other) : value_(other.value_) {
    secure_clear_text(other.value_);
}

WipedLine& WipedLine::operator=(WipedLine&& other) {
    if (this != &other) {
        secure_clear_text(value_);
        value_ = other.value_;
        secure_clear_text(other.value_);
    }
    return *this;
}

WipedLine::~WipedLine() { secure_clear_text(value_); }

HubCommand::HubCommand(HubCommand&& other)
    : type(other.type), id_or_generation(other.id_or_generation), room_id(other.room_id), json(std::move(other.json)) {
    secure_clear_text(other.room_id);
    secure_clear_bytes(other.json);
}

HubCommand& HubCommand::operator=(HubCommand&& other) {
    if (this != &other) {
        clear();
        type = other.type;
        id_or_generation = other.id_or_generation;
        room_id = other.room_id;
        json = std::move(other.json);
        secure_clear_text(other.room_id);
        secure_clear_bytes(other.json);
    }
    return *this;
}

HubCommand::~HubCommand() { clear(); }

void HubCommand::clear() noexcept {
    secure_clear_bytes(json);
    secure_clear_text(room_id);
    type = HubCommandType{};
    id_or_generation = 0;
}

HubParseResult HubCommandParser::parse(std::string_view line) {
    return parse_command(line, channel_, &ice_seen_);
}

HubParseResult parse_rooms_command(std::string_view line) {
    bool ice_seen = false;
    return parse_command(line, HubCommandChannel::rooms, &ice_seen);
}

HubParseResult parse_hub_command(std::string_view line) {
    bool ice_seen = false;
    return parse_command(line, HubCommandChannel::hub, &ice_seen);
}

std::string format_rooms_command(const HubCommand& command) {
    return format_command(HubCommandChannel::rooms, command);
}

std::string format_hub_command(const HubCommand& command) {
    return format_command(HubCommandChannel::hub, command);
}

bool HubCommandFramer::enqueue(std::string&& line) {
    if (lines_.size() >= max_queued_lines || queued_bytes_ > max_queued_bytes ||
        line.size() > max_queued_bytes - queued_bytes_) {
        secure_clear_text(line);
        return false;
    }
    queued_bytes_ += line.size();
    lines_.emplace_back(std::move(line));
    return true;
}

bool HubCommandFramer::fail(Error error) {
    stopped_ = true;
    error_ = error;
    clear();
    error_ = error;
    return false;
}

HubCommandFramer::~HubCommandFramer() { clear(); }

void HubCommandFramer::clear() noexcept {
    secure_clear_text(pending_);
    lines_.clear();
    queued_bytes_ = 0;
}

bool HubCommandFramer::feed(std::string_view bytes) {
    if (stopped_) return false;
    while (!bytes.empty()) {
        const auto newline = bytes.find('\n');
        if (newline == std::string_view::npos) {
            if (pending_.size() > max_line_bytes || bytes.size() > max_line_bytes - pending_.size()) return fail(Error::line_too_long);
            pending_.append(bytes.data(), bytes.size());
            return true;
        }
        if (pending_.size() > max_line_bytes || newline > max_line_bytes - pending_.size()) return fail(Error::line_too_long);
        pending_.append(bytes.data(), newline);
        bytes.remove_prefix(newline + 1U);
        if (!enqueue(std::move(pending_))) return fail(Error::queue_overflow);
        pending_.clear();
    }
    return true;
}

bool HubCommandFramer::finish() {
    if (stopped_) return false;
    if (!pending_.empty() && !enqueue(std::move(pending_))) return fail(Error::queue_overflow);
    pending_.clear();
    return true;
}

std::deque<WipedLine> HubCommandFramer::drain() {
    auto result = std::move(lines_);
    lines_.clear();
    queued_bytes_ = 0;
    return result;
}

ParseResult parse_child_event(std::string_view line) {
    if (line.starts_with(hub_prefix)) return parse_v2_event(line);
    return parse_v1_event(line);
}

ParseResult ChildEventParser::parse(std::string_view line) {
    const int candidate = line.starts_with(v1_prefix) ? 1 : line.starts_with(hub_prefix) ? 2 : 0;
    if (candidate == 0 || (version_ != 0 && candidate != version_)) return {{}, "child protocol version changed in stream"};
    auto parsed = parse_child_event(line);
    if (parsed.event && version_ == 0) version_ = candidate;
    return parsed;
}

std::string format_child_event(ChildEventType type,
                               const std::map<std::string, std::string, std::less<>>& fields) {
    if (type == ChildEventType::fatal) return {};
    if (!valid_hub_event_fields(type, fields)) return {};
    std::string out = "Q2DZ/HUB/2 ";
    switch (type) {
        case ChildEventType::transport_ready: out += "READY"; break;
        case ChildEventType::room_closed: out += "CLOSED"; break;
        case ChildEventType::fatal: out += "FAILED"; break;
        case ChildEventType::target_failed: out += "FAILED"; break;
        case ChildEventType::heartbeat: out += "HEARTBEAT"; break;
    }
    append_hub_event_fields(out, type, fields);
    if (!parse_v2_event(out).event || out.size() > max_hub_line_bytes) return {};
    return out;
}

bool ChildEventFramer::enqueue(std::string&& line) {
    if (lines_.size() + 1 >= max_queued_lines || queued_bytes_ > max_queued_bytes - max_fatal_line_bytes)
        return false;
    if (line.size() > max_queued_bytes - max_fatal_line_bytes - queued_bytes_) return false;
    queued_bytes_ += line.size();
    lines_.push_back(std::move(line));
    return true;
}

bool ChildEventFramer::fail(Error error) {
    if (stopped_) return false;
    stopped_=true;
    error_=error;
    pending_.clear();
    const auto line=fatal_line(error);
    lines_.emplace_back(line);
    queued_bytes_ += line.size();
    return false;
}

bool ChildEventFramer::feed(std::string_view bytes) {
    if (stopped_) return false;
    while (!bytes.empty()) {
        const auto newline=bytes.find('\n');
        if (newline==std::string_view::npos) {
            if (bytes.size() > max_line_bytes-pending_.size()) return fail(Error::line_too_long);
            pending_.append(bytes.data(), bytes.size());
            return true;
        }
        if (newline >= max_line_bytes-pending_.size()) return fail(Error::line_too_long);
        pending_.append(bytes.data(), newline+1);
        bytes.remove_prefix(newline+1);
        if (!enqueue(std::move(pending_))) return fail(Error::queue_overflow);
        pending_.clear();
    }
    return true;
}

bool ChildEventFramer::finish() {
    if (stopped_) return false;
    if (pending_.empty()) return true;
    if (!enqueue(std::move(pending_))) return fail(Error::queue_overflow);
    pending_.clear();
    return true;
}

std::deque<std::string> ChildEventFramer::drain() {
    auto result=std::move(lines_);
    lines_.clear();
    queued_bytes_=0;
    return result;
}

std::string LoopbackEndpoint::text() const {
    return "127.0.0.1:" + std::to_string(port);
}

bool field_name_is_forbidden(std::string_view name) noexcept {
    const auto n = lower(name);
    constexpr std::string_view forbidden[] = {
        "ice", "credential", "password", "secret", "token", "username",
        "candidate", "sdp", "authorization", "cookie"
    };
    for (const auto x : forbidden) if (n.find(x) != std::string::npos) return true;
    return false;
}

std::string percent_decode(std::string_view v, bool* ok) {
    bool valid = true;
    std::string out;
    out.reserve(v.size());
    for (std::size_t i=0; i<v.size(); ++i) {
        if (v[i] == '%') {
            if (i+2 >= v.size()) { valid=false; break; }
            const int hi=hex_value(v[i+1]), lo=hex_value(v[i+2]);
            if (hi<0 || lo<0) { valid=false; break; }
            const char c=static_cast<char>((hi<<4)|lo);
            if (c == '\0' || c == '\r' || c == '\n') { valid=false; break; }
            out.push_back(c); i+=2;
        } else {
            const unsigned char c=static_cast<unsigned char>(v[i]);
            if (c < 0x20 || c == 0x7f) { valid=false; break; }
            out.push_back(static_cast<char>(c));
        }
    }
    if (ok) *ok=valid;
    if (!valid) return {};
    return out;
}

std::optional<LoopbackEndpoint> parse_loopback_endpoint(std::string_view text) {
    constexpr std::string_view host="127.0.0.1:";
    if (!text.starts_with(host)) return std::nullopt;
    const auto p=text.substr(host.size());
    if (p.empty() || p.size()>5) return std::nullopt;
    unsigned port=0;
    const auto [end,ec]=std::from_chars(p.data(), p.data()+p.size(), port);
    if (ec != std::errc{} || end != p.data()+p.size() || port < 1024 || port > 65535)
        return std::nullopt;
    return LoopbackEndpoint{static_cast<std::uint16_t>(port)};
}

} // namespace q2dz
